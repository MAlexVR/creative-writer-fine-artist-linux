#!/bin/sh
# User-level installer for Creative Writer and Fine Artist (no root, nothing outside your home directory):
#   cwfa-install [--cd CD_DIR_OR_ISO] [--es-cd DIR] [--es-fa-cd DIR] [--icons auto|original|plain]
#   cwfa-install --uninstall [--purge] [--yes]
# From a checkout: scripts/install.sh builds the portable package and installs it. From the package it is
# "cwfa-portable.run --install ..." (or "cwfa-launch --install ..." once installed).
#
# Install puts these under your home directory, and nothing else:
#   ~/.local/share/cwfa/app/                          the runtime (binary, libraries, fonts, launcher)
#   ~/.local/bin/cwfa-launch                          a link to the launcher
#   ~/.local/share/applications/cwfa-<program>.desktop   Creative Writer and Fine Artist in the app launcher
#                                                     (Spanish editions too when their media are given)
#   ~/.local/share/icons/hicolor/<N>x<N>/apps/cwfa-<program>.png   their icons, N = 32 64 128 256
#   ~/.config/cwfa/media.conf                         where your CD / ISO / Spanish disks are (asked once by the launcher)
#   ~/.local/share/cwfa/install.manifest              what was written, so uninstall removes exactly that
# Your users and documents live in ~/.local/share/cwfa/data (English) and data-es (Spanish): the launcher keeps them there
# (--user-data); uninstall leaves them alone unless --purge (it asks first; --yes answers for you).
# The game itself is never part of the install: point it at your own CD with --cd (a directory or the .ISO) or let the
# launcher ask on the first start. Installing again updates in place.
set -e

main() {
    here=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
    data=${XDG_DATA_HOME:-$HOME/.local/share}
    conf=${XDG_CONFIG_HOME:-$HOME/.config}/cwfa
    base=$data/cwfa
    app=$base/app
    manifest=$base/install.manifest
    bindir=$HOME/.local/bin
    mode=install; cd_arg=; es_arg=; fa_arg=; style=auto; purge=; yes=
    while [ $# -gt 0 ]; do
        case "$1" in
            --uninstall) mode=uninstall; shift ;;
            --purge) purge=1; shift ;;
            --yes|-y) yes=1; shift ;;
            --cd) [ $# -ge 2 ] || usage; cd_arg=$2; shift 2 ;;
            --es-cd) [ $# -ge 2 ] || usage; es_arg=$2; shift 2 ;;
            --es-fa-cd) [ $# -ge 2 ] || usage; fa_arg=$2; shift 2 ;;
            --icons) [ $# -ge 2 ] || usage; style=$2; shift 2 ;;
            -h|--help) usage ;;
            *) usage ;;
        esac
    done
    case "$style" in auto|original|plain) ;; *) usage ;; esac
    if [ "$mode" = uninstall ]; then do_uninstall; else do_install; fi
}

usage() {
    cat >&2 <<EOF
usage: cwfa-install [--cd CD_DIR_OR_ISO] [--es-cd DIR] [--es-fa-cd DIR] [--icons auto|original|plain]
       cwfa-install --uninstall [--purge] [--yes]
  --cd PATH       the English CD (a directory of its files, or the .ISO)
  --es-cd DIR     the Spanish Creative Writer media;  --es-fa-cd DIR  the Spanish Fine Artist media
  --icons STYLE   plain (the Spanish editions' icons, no CD drawn on them) is what auto picks when Spanish media are given
  --uninstall     remove what install wrote; your documents stay
  --purge --yes   with --uninstall: also delete the users and documents
EOF
    exit 2
}

absp() {   # absolute path of an existing file or directory
    if [ -d "$1" ]; then (cd "$1" && pwd -P)
    elif [ -f "$1" ]; then printf '%s/%s\n' "$(cd "$(dirname "$1")" && pwd -P)" "$(basename "$1")"
    else echo "cwfa-install: $1 does not exist" >&2; return 1
    fi
}

conf_set() {   # conf_set KEY VALUE: one key=value line in media.conf
    mkdir -p "$conf"
    touch "$conf/media.conf"
    grep -v "^$1=" "$conf/media.conf" > "$conf/media.conf.new" || true
    printf '%s=%s\n' "$1" "$2" >> "$conf/media.conf.new"
    mv "$conf/media.conf.new" "$conf/media.conf"
}
conf_get() { sed -n "s/^$1=//p" "$conf/media.conf" 2>/dev/null | head -n 1; }

do_install() {
    if [ ! -x "$here/bin/cwfa" ]; then
        # a checkout: build the package and install from it
        if [ -f "$here/package.sh" ]; then
            dist=${CWFA_DIST:-$here/../dist}
            sh "$here/package.sh" "$dist" >&2
            run=$(ls "$dist"/cwfa-portable-*.run | head -n 1)
            set -- --install
            [ -z "$cd_arg" ] || set -- "$@" --cd "$cd_arg"
            [ -z "$es_arg" ] || set -- "$@" --es-cd "$es_arg"
            [ -z "$fa_arg" ] || set -- "$@" --es-fa-cd "$fa_arg"
            exec "$run" "$@" --icons "$style"
        fi
        echo "cwfa-install: no runtime next to this script (bin/cwfa)" >&2
        exit 1
    fi
    [ -z "$cd_arg" ] || cd_arg=$(absp "$cd_arg")
    [ -z "$es_arg" ] || es_arg=$(absp "$es_arg")
    [ -z "$fa_arg" ] || fa_arg=$(absp "$fa_arg")

    # directories that do not exist yet are ours to remove again
    newdirs=$(mktemp "${TMPDIR:-/tmp}/cwfa-newdirs.XXXXXX")
    for d in "$base" "$conf" "$HOME/.local" "$bindir" "$data/applications" "$data/icons" "$data/icons/hicolor" \
             "$data/icons/hicolor/32x32" "$data/icons/hicolor/32x32/apps" "$data/icons/hicolor/64x64" "$data/icons/hicolor/64x64/apps" \
             "$data/icons/hicolor/128x128" "$data/icons/hicolor/128x128/apps" "$data/icons/hicolor/256x256" "$data/icons/hicolor/256x256/apps"; do
        [ -d "$d" ] || printf '%s\n' "$d" >> "$newdirs"
    done

    # the runtime, copied in place (the .run's cache may be cleaned any time)
    mkdir -p "$base"
    if [ "$here" != "$app" ]; then
        tmp=$base/.app.$$
        rm -rf "$tmp"
        mkdir -p "$tmp"
        cp -a "$here/." "$tmp/"
        rm -rf "$app"
        mv "$tmp" "$app"
    fi
    mkdir -p "$bindir"
    ln -sfn "$app/cwfa-launch" "$bindir/cwfa-launch"

    # the media the launcher will use
    [ -z "$cd_arg" ] || conf_set cd "$cd_arg"
    [ -z "$es_arg" ] || conf_set es_cw "$es_arg"
    [ -z "$fa_arg" ] || conf_set es_fa "$fa_arg"
    cd_dir=$(conf_get cd); es_dir=$(conf_get es_cw); fa_dir=$(conf_get es_fa)
    # an ISO is extracted by the launcher on the first start, which also makes the icons then
    [ -f "$cd_dir" ] && cd_dir=

    mkdir -p "$data/applications"
    pre_caches=$(mktemp "${TMPDIR:-/tmp}/cwfa-caches.XXXXXX")
    for c in "$data/applications/mimeinfo.cache" "$data/icons/hicolor/icon-theme.cache"; do   # written by the desktop's cache tools
        [ -e "$c" ] || printf '%s\n' "$c" >> "$pre_caches"
    done
    LD_LIBRARY_PATH="$app/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" "$app/cwfa-install-desktop" \
        --cwfa "$app/bin/cwfa" --launcher "$app/cwfa-launch" --media-conf --icons "$style" \
        --cd "$cd_dir" --es-cd "$es_dir" --es-fa-cd "$fa_dir" --quiet

    # what was written, for uninstall
    {
        [ ! -f "$manifest" ] || grep '^dir \|^cache ' "$manifest" || true
        while IFS= read -r d; do [ ! -d "$d" ] || printf 'dir %s\n' "$d"; done < "$newdirs"
        while IFS= read -r c; do [ ! -e "$c" ] || printf 'cache %s\n' "$c"; done < "$pre_caches"
        printf 'tree %s\nlink %s\n' "$app" "$bindir/cwfa-launch"
        for id in writer artist es-writer es-artist; do
            [ ! -e "$data/applications/cwfa-$id.desktop" ] || printf 'file %s\n' "$data/applications/cwfa-$id.desktop"
            [ ! -e "$data/icons/hicolor/.cwfa-icons-$id" ] || printf 'file %s\n' "$data/icons/hicolor/.cwfa-icons-$id"
            for z in 32 64 128 256; do
                [ ! -e "$data/icons/hicolor/${z}x${z}/apps/cwfa-$id.png" ] || printf 'file %s\n' "$data/icons/hicolor/${z}x${z}/apps/cwfa-$id.png"
            done
        done
        [ ! -e "$conf/media.conf" ] || printf 'file %s\n' "$conf/media.conf"
        printf 'file %s\n' "$manifest"
    } | sort -u > "$manifest.new"
    mv "$manifest.new" "$manifest"
    rm -f "$newdirs" "$pre_caches"

    echo "cwfa: installed under $app"
    echo "cwfa: Creative Writer and Fine Artist are in your application launcher; or run cwfa-launch writer | artist"
    if [ -z "$(conf_get cd)$(conf_get es_cw)$(conf_get es_fa)" ]; then
        echo "cwfa: no CD configured yet: the first start asks for it (or: cwfa-launch --configure)"
    fi
    echo "cwfa: your users and documents will be in $base/data (and data-es for the Spanish editions)"
    echo "cwfa: to uninstall: cwfa-launch --uninstall   (your documents are kept; --purge --yes removes them too)"
}

do_uninstall() {
    m=
    if [ -f "$manifest" ]; then
        m=$(mktemp "${TMPDIR:-/tmp}/cwfa-manifest.XXXXXX")
        cp "$manifest" "$m"
        while IFS= read -r line; do
            kind=${line%% *}; path=${line#* }
            case "$kind" in
                file) rm -f "$path" ;;
                link) [ ! -L "$path" ] || rm -f "$path" ;;
                tree) case "$path" in */cwfa/app) rm -rf "$path" ;; esac ;;
            esac
        done < "$m"
    else
        # no manifest (a partial install): only the names this installer uses
        rm -f "$bindir/cwfa-launch" 2>/dev/null || true
        for id in writer artist es-writer es-artist; do
            rm -f "$data/applications/cwfa-$id.desktop" "$data/icons/hicolor/.cwfa-icons-$id"
            for z in 32 64 128 256; do rm -f "$data/icons/hicolor/${z}x${z}/apps/cwfa-$id.png"; done
        done
        rm -f "$conf/media.conf"
        case "$app" in */cwfa/app) rm -rf "$app" ;; esac
    fi
    # the extracted CD image cache is ours too (it is made again when needed)
    case "$base" in */cwfa) rm -rf "$base/cd" ;; esac
    command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$data/applications" >/dev/null 2>&1 || true
    command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -q -t "$data/icons/hicolor" >/dev/null 2>&1 || true
    if [ -n "$m" ]; then
        # cache files install made appear (not there before), then the directories it created, deepest first, when empty
        grep '^cache ' "$m" | sed 's/^cache //' | while IFS= read -r c; do rm -f "$c"; done || true
        grep '^dir ' "$m" | sed 's/^dir //' | sort -r | while IFS= read -r d; do rmdir "$d" 2>/dev/null || true; done
        rm -f "$m"
    fi
    rm -f "$manifest"
    rmdir "$conf" 2>/dev/null || true

    set --
    for d in "$base/data" "$base/data-es"; do [ ! -d "$d" ] || set -- "$@" "$d"; done
    if [ $# -gt 0 ] && [ -n "$purge" ]; then
        ok=
        if [ -n "$yes" ]; then ok=1
        elif [ -t 0 ]; then
            printf 'cwfa: delete your users and documents in %s? [y/N] ' "$*" >&2
            read -r ans || ans=
            case "$ans" in y|Y|yes|YES) ok=1 ;; esac
        else
            echo "cwfa: not deleting your documents without a terminal: add --yes to confirm" >&2
        fi
        if [ -n "$ok" ]; then
            for d in "$@"; do case "$d" in */cwfa/data|*/cwfa/data-es) rm -rf "$d" ;; esac; done
            set --
        fi
    fi
    rmdir "$base" 2>/dev/null || true
    echo "cwfa: uninstalled (the launcher entries, icons, runtime and settings are gone)"
    if [ $# -gt 0 ]; then
        echo "cwfa: kept your documents in: $*"
        echo "cwfa: delete them with: cwfa-install --uninstall --purge --yes (or remove those folders)"
    fi
}

main "$@"
exit 0
