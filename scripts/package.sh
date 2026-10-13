#!/bin/sh
# Builds the portable package: one self-extracting executable (dist/cwfa-portable-<arch>.run) with the cwfa binary,
# the SDL2 libraries it needs and the launcher. The package contains no game data: the user supplies the CD (a
# directory or the .ISO) at run time:  ./cwfa-portable-x86_64.run writer --cd /path/to/CWFA110__01.ISO
#
# Linking: SDL2 is linked dynamically (this machine has no static libSDL2), so libSDL2 is bundled, and so is libSDL3 when
# the system SDL2 is the sdl2-compat shim (Fedora 40+), which loads libSDL3.so.0 at run time. glibc is NOT bundled:
# the package runs on a glibc at least as new as the one it was built on. Video and audio drivers (X11, Wayland,
# PipeWire/PulseAudio, ALSA) are loaded by SDL from the host at run time.
# usage: scripts/package.sh [OUTDIR]       (default dist)
set -e
cd "$(dirname "$0")/.."
out=${1:-dist}
arch=$(uname -m)
make >/dev/null
[ -x build/cwfa ] || { echo "package: build/cwfa missing" >&2; exit 1; }

stage=$(mktemp -d "${TMPDIR:-/tmp}/cwfa-pkg.XXXXXX")
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/pkg/bin" "$stage/pkg/lib"
cp build/cwfa "$stage/pkg/bin/cwfa"
cp scripts/launch.sh "$stage/pkg/cwfa-launch"
cp scripts/install-desktop.sh "$stage/pkg/cwfa-install-desktop"
cp scripts/install.sh "$stage/pkg/cwfa-install"
chmod +x "$stage/pkg/cwfa-install-desktop" "$stage/pkg/cwfa-install"
mkdir -p "$stage/pkg/fonts"          # Liberation faces (SIL OFL) for Arial/Times/Courier: found by cwfa as <bin>/../fonts
cp third_party/fonts/*.ttf third_party/fonts/LICENSE-Liberation-OFL.txt "$stage/pkg/fonts/"
chmod +x "$stage/pkg/cwfa-launch"

# the shared libraries cwfa needs, except libc/libm/ld.so (the host's)
ldd build/cwfa | awk '/=> \// {print $3}' | while read -r lib; do
    case "$(basename "$lib")" in
        libc.so*|libm.so*|libdl.so*|libpthread.so*|librt.so*|ld-linux*) ;;
        *) cp -L "$lib" "$stage/pkg/lib/" ;;
    esac
done
sdl2=$(ls "$stage"/pkg/lib/libSDL2-2.0.so.0 2>/dev/null || true)
if [ -n "$sdl2" ] && grep -aq 'libSDL3.so.0' "$sdl2"; then          # sdl2-compat: SDL3 is dlopen()ed
    for d in /usr/lib64 /usr/lib /usr/lib/x86_64-linux-gnu /usr/lib/aarch64-linux-gnu; do
        [ -e "$d/libSDL3.so.0" ] && { cp -L "$d/libSDL3.so.0" "$stage/pkg/lib/"; break; }
    done
    [ -e "$stage/pkg/lib/libSDL3.so.0" ] || echo "package: warning: libSDL3.so.0 not found; the target needs SDL3 installed" >&2
fi
cat > "$stage/pkg/README.txt" <<EOF
Creative Writer & Fine Artist, native runtime (cwfa). No game data is included.
Run:  ./$(basename "$0" .sh)  (the .run file)  writer|artist --cd /path/to/CD_or_ISO
Install for your user (app launcher entries, icons, remembered CD):  ./<the .run file> --install --cd /path/to/CD_or_ISO
  then start Creative Writer / Fine Artist from the launcher, or:  cwfa-launch writer | artist | es-writer | es-artist
Remove it again (your documents stay):  cwfa-launch --uninstall      (--purge --yes also deletes them)
Only the menu entries and icons for a CD:  ./<the .run file> --install-desktop --cd /path/to/CD_or_ISO  (--remove-desktop undoes it)
Printing writes PDF files (virtual "PDF Printer") to <your Documents folder>/Creative Writer; choose another with --print-dir DIR.
EOF

tar -C "$stage/pkg" -cJf "$stage/payload.tar.xz" .
sum=$(sha256sum "$stage/payload.tar.xz" | cut -c1-16)
mkdir -p "$out"
run="$out/cwfa-portable-$arch.run"
cat > "$run" <<EOF
#!/bin/sh
# Self-extracting package of cwfa (Creative Writer & Fine Artist native runtime); see scripts/package.sh.
set -e
cache=\${XDG_CACHE_HOME:-\$HOME/.cache}/cwfa-portable/$sum
if [ ! -x "\$cache/cwfa-launch" ]; then
    mkdir -p "\$cache.tmp\$\$"
    tail -n +PAYLOAD_LINE "\$0" | tar -xJ -C "\$cache.tmp\$\$"
    rm -rf "\$cache"
    mv "\$cache.tmp\$\$" "\$cache"
fi
CWFA_PACKAGE=\$(readlink -f "\$0") exec "\$cache/cwfa-launch" "\$@"
exit 1
__PAYLOAD_BELOW__
EOF
line=$(($(wc -l < "$run") + 1))
sed -i "s/PAYLOAD_LINE/$line/" "$run"
cat "$stage/payload.tar.xz" >> "$run"
chmod +x "$run"
echo "package: $run ($(du -h "$run" | cut -f1)), payload $sum; libraries: $(ls "$stage/pkg/lib" | tr '\n' ' ')"
